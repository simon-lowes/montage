// Montage — File › Multiviewer: up to 16 live inputs watched side by side, each any feed File › Capture records (SMPTE
// ST 2110 streams from their SDP files, SRT, RIST or UDP streams, an Open Media Transport source, a camera or capture
// card). Each tile shows its picture with its label, its tally (red while it records, green while it is on the Source
// monitor), its sound's meters and its timecode. The inputs chosen are recorded while Record is on, each into the
// capture folder and the Captures bin as File › Capture records them (growing there as they record); a tile
// double-clicked is watched on the Source monitor, where In and Out are marked live and the range cut in. An input
// whose feed stops is waited for again.
#pragma once

#include <QImage>
#include <QJsonObject>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

#include "core/Model.h"

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace montage {

class EditorState;
class LiveFeed;
class MultiviewTile;

class Multiviewer : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMaxInputs = 16;
    struct Input {
        QString label;    // "" its address
        QString kind;     // as File › Capture's feeds: "" a URL, "st2110" (SDP files apart by ';', or SDP text), "omt", a device kind
        QString url;
        QString options;  // "name=value" apart by spaces (FFmpeg's input options; for st2110 interface= and interface2=)
        bool record = false;  // chosen: recorded while Record is on
    };
    // An input as it is now.
    struct Tile {
        Input input;
        bool running = false;    // its pictures coming
        bool recording = false;  // being recorded
        bool onSource = false;   // on the Source monitor
        QString timecode;        // the picture shown's
        int64_t pictures = 0;    // seen since it was (re)opened
        int width = 0, height = 0;
        std::vector<float> peaks;
        QString path;            // being or last recorded
        Id media = 0;            // its media item, once imported
        QString error;           // why it is not showing
    };

    explicit Multiviewer(EditorState* state, QWidget* parent = nullptr);
    ~Multiviewer() override;

    // An input added and watched at once (recorded too when chosen while Record is on). False with why at 16 inputs, or
    // with options that are not name=value.
    bool addInput(const Input& in, QString* error = nullptr);
    bool removeInput(int i);
    void clear();
    int inputCount() const { return int(inputs_.size()); }
    Tile tile(int i) const;
    std::shared_ptr<LiveFeed> feed(int i) const;
    MultiviewTile* tileWidget(int i) const;
    // Recording: the chosen inputs, each to "<label> 001.mov" (the next number free) in the folder, in the codec,
    // imported into the Captures bin a second in. Choosing an input or not while Record is on starts or ends its
    // recording (the feed opened again for it).
    void setChosen(int i, bool record);
    bool startRecording(QString* error = nullptr);
    void stopRecording();
    bool isRecording() const { return recording_; }
    void setFolder(const QString& dir);
    QString folder() const;
    void setCodec(const QString& id);
    // The input shown on the Source monitor (-1: none): its tally green.
    void setOnSource(int i);
    int onSource() const { return onSource_; }
    // What the Source monitor shows now (one of these inputs' feeds, or another): the tallies follow.
    void sourceShowing(const std::shared_ptr<LiveFeed>& feed);
    QString statusText() const;

    // montage_capture's "multiview".
    QString runTool(const QJsonObject& args, QJsonObject& structured, bool& isError);

signals:
    // An input to watch on the Source monitor (on), or no longer (off: its feed, if it is the one shown).
    void showOnSource(std::shared_ptr<montage::LiveFeed> feed, montage::Id media, const QString& label, bool recording, bool on);
    void changed();

private:
    struct Slot;
    bool startSlot(Slot& s, bool record, QString* error = nullptr);
    void importSlot(Slot& s);
    void slotFinished(quint64 id, int generation, bool ok, const QString& message);
    Slot* find(quint64 id) const;
    int indexOf(const Slot* s) const;
    void tick();
    void relayout();
    void announce(Slot& s);  // the Source monitor told what it shows of `s` now
    QString nextPath(const QString& label) const;
    bool addInteractive();
    void say(const QString& text);  // in the status line for a few seconds

    EditorState* state_;
    std::vector<std::unique_ptr<Slot>> inputs_;
    quint64 nextId_ = 1;
    bool recording_ = false;
    int onSource_ = -1;
    QGridLayout* grid_;
    QWidget* wall_;
    QPushButton* record_;
    QLineEdit* folder_;
    QComboBox* codec_;
    QLabel* status_;
    QTimer* timer_;
    int ticks_ = 0;
    qint64 messageUntil_ = 0;
};

// One input's tile: its picture, label, tally, meters and timecode (drawn from Multiviewer::Tile).
class MultiviewTile : public QWidget {
public:
    explicit MultiviewTile(QWidget* parent = nullptr);
    void show(const Multiviewer::Tile& t, const QImage& picture);
    const Multiviewer::Tile& state() const { return tile_; }
    QCheckBox* chooser() const { return choose_; }
    QColor tally() const;  // red recording, green on the Source monitor, else none (invalid)
    std::function<void()> onOpen;           // double-clicked
    std::function<void(QPoint)> onMenu;     // right-clicked, at
    QSize sizeHint() const override { return {320, 200}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    Multiviewer::Tile tile_;
    QImage picture_;
    QCheckBox* choose_;
};

}  // namespace montage
