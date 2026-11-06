// Montage — deck control in the app (control/NinePin.h, control/DeckCapture.h, audio/LtcChase.h): a deck on a serial
// port cued, shuttled and read; batch capture from a log (ALE, CSV or the Program's marked ranges) into the project;
// edit to tape, the Program played out through Reference Output as the deck records; Montage answering as a deck so an
// edit controller or a DAW's machine control drives the Program; and the Program chasing LTC on a sound input.
// The agent link's montage_deck does the same.
#pragma once

#include <QDialog>
#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QPointer>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "audio/LtcChase.h"
#include "control/DeckCapture.h"
#include "control/NinePin.h"
#include "core/Model.h"

class QAudioSource;
class QIODevice;
class QTimer;
class QLabel;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QPlainTextEdit;

namespace montage {

class EditorState;
class PlaybackController;

class DeckController : public QObject {
    Q_OBJECT
public:
    DeckController(EditorState* state, PlaybackController* program, QObject* parent = nullptr);
    ~DeckController() override;

    // ---- A deck under control
    bool connectDeck(const QString& port, Rational rate, QString* error = nullptr);
    void disconnectDeck();
    bool deckConnected() const;
    ninepin::Deck& deck() { return deck_; }
    Rational deckRate() const { return deckRate_; }
    // "Play 10:00:01:12 (LTC)", or why it cannot be read.
    QString deckStatus();

    // ---- Work on the deck, one job at a time on a thread of its own (workProgress and workFinished report it).
    // Batch capture: each entry captured to `s.folder` and imported into a bin named for its tape, the tape kept in
    // the media's log.
    bool startBatchCapture(const std::vector<LogEntry>& log, const BatchCaptureSettings& s, QString* error = nullptr);
    // Edit to tape: the Program from `programIn` laid onto the tape at e.in to e.out, played out as the deck records.
    bool startEditToTape(const TapeEdit& e, FrameTime programIn, QString* error = nullptr);
    // The deck seen running up to the edit's In, `framesToIn` before it at `clock` (captureClock()): the Program put
    // where it reaches `programIn` as the deck reaches In, and started (now, or on its first frame at its time). A
    // later roll replaces it, a start still to come included.
    void rollProgram(FrameTime programIn, int64_t framesToIn, double clock);
    bool rollPending() const;
    bool working() const { return worker_.joinable() && !workDone_; }
    void cancelWork();
    QString lastWork() const { return lastWork_; }
    const std::vector<BatchClip>& lastCaptures() const { return captured_; }

    // ---- Montage as a deck: the Program driven by an edit controller or a DAW on `port`.
    bool startRemote(const QString& port, QString* error = nullptr);
    void stopRemote();
    bool remoteRunning() const;
    // The Program's timecode at its first frame (as the controller sees it), a frame number at its rate.
    void setProgramStart(int64_t frame) { programStart_ = frame; }
    int64_t programStart() const { return programStart_; }

    // ---- The Program chasing LTC on a sound input (its description, "" for the default) and channel.
    bool startChase(const QString& input, int channel, int64_t offsetFrames, double freewheelSeconds, QString* error = nullptr);
    void stopChase();
    bool chasing() const { return chase_ != nullptr; }
    // Samples as they come from the input (interleaved), for the input and the tests.
    void feedChase(const float* samples, int frames, int channels);
    LtcChase::Position chasePosition() const;
    QString chaseStatus() const;

    static QByteArray toolJson();
    QString runTool(const QJsonObject& args, QJsonObject& structured, bool& isError);

signals:
    void workProgress(const QString& what);
    void workFinished(bool ok, const QString& message);
    void remoteChanged(bool on);
    void chaseChanged(bool on);

private:
    void publish();
    void chaseTick();
    LtcChase::Position chaseNow() const;
    void finishWork();
    int base() const;
    bool dropFrame() const;

    EditorState* state_;
    QPointer<PlaybackController> program_;  // (gone first at shutdown)
    ninepin::Deck deck_;
    Rational deckRate_{25, 1};
    std::thread worker_;
    std::atomic<bool> cancel_{false}, workDone_{true};
    QString lastWork_;
    std::vector<BatchClip> captured_;
    std::unique_ptr<ninepin::DeckEmulator> remote_;
    int64_t programStart_ = 0;
    std::unique_ptr<LtcChase> chase_;
    QAudioSource* chaseSource_ = nullptr;
    QIODevice* chaseIo_ = nullptr;
    int chaseChannel_ = 0, chaseChannels_ = 1;
    QTimer* chaseTimer_ = nullptr;
    QTimer* rollTimer_ = nullptr;  // (the Program's start, when it comes after the deck's roll)
    bool chaseRolling_ = false;
    QByteArray chasePending_;  // a sample frame split between reads
    double chaseRate_ = 48000;
    std::chrono::steady_clock::time_point chaseFed_;
    QString statusError_;
    std::chrono::steady_clock::time_point statusFailed_;
};

// The Deck Control window: the deck's transport and timecode, batch capture, edit to tape, Montage as a deck and LTC
// chase.
class DeckDialog : public QDialog {
    Q_OBJECT
public:
    DeckDialog(DeckController* deck, EditorState* state, QWidget* parent = nullptr);

private:
    void refresh();
    void connectDeck();
    void runBatch();
    void runEdit();
    void toggleRemote();
    void toggleChase();

    DeckController* deck_;
    EditorState* state_;
    QComboBox* port_ = nullptr;
    QComboBox* rate_ = nullptr;
    QPushButton* connect_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* cue_ = nullptr;
    QLineEdit* log_ = nullptr;
    QLineEdit* source_ = nullptr;
    QComboBox* sourceKind_ = nullptr;
    QLineEdit* folder_ = nullptr;
    QDoubleSpinBox* handles_ = nullptr;
    QDoubleSpinBox* preroll_ = nullptr;
    QComboBox* timeSource_ = nullptr;
    QLineEdit* editIn_ = nullptr;
    QCheckBox* assemble_ = nullptr;
    QCheckBox* video_ = nullptr;
    std::vector<QCheckBox*> audio_;
    QComboBox* remotePort_ = nullptr;
    QLineEdit* programStart_ = nullptr;
    QPushButton* remote_ = nullptr;
    QComboBox* chaseInput_ = nullptr;
    QSpinBox* chaseChannel_ = nullptr;
    QSpinBox* chaseOffset_ = nullptr;
    QPushButton* chase_ = nullptr;
    QPlainTextEdit* activity_ = nullptr;
    QTimer* tick_ = nullptr;
};

}  // namespace montage
