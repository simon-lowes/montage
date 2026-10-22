// Montage — the Sound Library panel: the sound effects library (media/SoundLibrary.h) searched as you type, by name,
// UCS category, description and tags, or by what it sounds like (CLAP), and Find More Like This from any sound.
// A sound's waveform shows below the list; drag across it to choose a stretch, Space plays it (or the whole sound)
// raised or lowered in pitch, by semitones or as varispeed would, and dragging the stretch onto the timeline places
// just that, to the sample.
#pragma once

#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <QFuture>

#include "core/Model.h"
#include "media/Decoder.h"
#include "media/SoundLibrary.h"

class QAudioSink;
class QBuffer;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMimeData;
class QPushButton;
class QTableWidget;
class QTimer;

namespace montage {

class EditorState;
class SoundWaveView;

// A stretch of a sound file dragged from the library: [first, first + count) samples at `rate` (the file's).
constexpr const char* kSoundRangeMime = "application/x-montage-sound";
struct SoundRange {
    QString path;
    int64_t first = 0, count = 0;
    int rate = 0;
};
QMimeData* soundRangeMime(const SoundRange& r);  // with the file's URL too, for other applications
bool soundRangeFromMime(const QMimeData* mime, SoundRange& out);

class SoundLibraryPanel : public QWidget {
    Q_OBJECT
public:
    // `library`: SoundLibrary::shared() when null.
    explicit SoundLibraryPanel(EditorState* state, std::shared_ptr<SoundLibrary> library = nullptr, QWidget* parent = nullptr);
    ~SoundLibraryPanel() override;

    SoundLibrary* library() const { return library_.get(); }
    bool addFolder(const QString& folder);  // and scans it
    void removeFolder(const QString& folder);
    // Brings the index up to date in the background (then listens to new sounds when the model is here); `wait`
    // blocks until it is done.
    void rescan(bool wait = false);
    bool busy() const;

    // The search box's modes: words (as you type) or what it sounds like (CLAP; the model offered first).
    enum Mode { Words = 0, SoundsLike = 1 };
    void setMode(Mode m);
    int search(const QString& text);  // in the current mode; the number found
    int moreLike(int64_t id);         // Find More Like This
    const std::vector<SoundLibraryEntry>& results() const { return results_; }

    // Shows result `row`'s waveform (the whole sound chosen); -1 for none.
    void select(int row);
    const SoundLibraryEntry* current() const;
    // The stretch chosen on the waveform, in samples at the file's rate (the whole sound when none is).
    SoundRange range() const;
    void setRange(int64_t first, int64_t count);
    // Audition: raised or lowered by semitones; with varispeed, faster or slower as well (as tape would).
    void setPitch(double semitones, bool varispeed);
    // What Space plays: the stretch at the pitch, interleaved stereo at 48 kHz.
    std::vector<float> auditionSound() const;
    void play();
    void stop();
    bool playing() const;

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void showResults();
    void refreshStatus();
    void startListening();
    void watchFolders(const QStringList& dirs);
    bool ensureHeard();

    EditorState* state_;
    std::shared_ptr<SoundLibrary> library_;
    QLineEdit* query_;
    QComboBox* mode_;
    QLabel* status_;
    QTableWidget* table_;
    SoundWaveView* wave_;
    QDoubleSpinBox* pitch_;
    QCheckBox* varispeed_;
    QCheckBox* copy_;
    QPushButton* folders_;
    QTimer* typing_;
    QTimer* rescanSoon_;
    QFileSystemWatcher* watcher_;
    std::vector<SoundLibraryEntry> results_;
    std::vector<float> scores_;  // a sounds-like search's
    int currentRow_ = -1;
    QPoint press_;  // where a row was pressed (dragged out from there)
    AudioBufferPtr sound_;  // the current sound at its own rate
    QFuture<void> work_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::shared_ptr<std::atomic<int>> heard_, toHear_;
    bool scanning_ = false, listening_ = false;
    int run_ = 0;  // which rescan is the latest
    QAudioSink* sink_ = nullptr;
    QBuffer* playing_ = nullptr;
};

}  // namespace montage
