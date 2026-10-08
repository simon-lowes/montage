// Montage — edit by transcript. In Sequence mode the panel shows what the
// cut says: click a word to go there, select words and delete them to cut
// them out (ripple), remove filler words and long pauses. In Source mode it
// shows the source clip's transcript: select words to mark In and Out, then
// insert or overwrite them into the timeline.
#pragma once

#include <QWidget>
#include <vector>

#include "core/Model.h"
#include "core/Transcript.h"
#include "render/PaperEdit.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTextEdit;
class QToolButton;

namespace montage {

class EditorState;

class TranscriptPanel : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Sequence, Source };
    explicit TranscriptPanel(EditorState* state, QWidget* parent = nullptr);

    Mode mode() const { return mode_; }
    void setMode(Mode m);
    // The words shown, in seconds (timeline seconds in Sequence mode, media seconds in Source mode).
    const std::vector<TranscriptWord>& words() const { return words_; }
    // Selects words [first, last] in the text.
    void selectWords(int first, int last);
    // Indices of the selected words (first, last), or (-1, -1).
    std::pair<int, int> selectedWords() const;
    // Finds `text` (case-insensitive, whole words); returns the number of matches and selects the next one.
    int find(const QString& text, bool backwards = false);

public slots:
    void deleteSelection();     // Sequence: ripple-delete the selected words
    void removeFillerWords();   // Sequence
    void removeRetakes();       // Sequence: keep only the last take where the speaker started again
    void bleepSelection();      // Sequence: a tone over the selected words, masked in the captions
    void bleepProfanity();      // Sequence: the same for every swear word found
    void removePauses(double minPause = 1.0, double keep = 0.3);  // Sequence
    void markSelection();       // Source: In/Out from the selection
    void insertSelection(bool overwrite);  // Source
    void setSourcePosition(montage::FrameTime frame);  // highlights the word under the source playhead
    // Source: renames a speaker of the source clip's transcript (one undo step).
    bool renameSpeaker(const QString& from, const QString& to);
    // Paper Edit (Source): the selected words join the list (from any clip), which can be reordered and
    // assembled into a new sequence, made active (one undo step). Returns the list's length / the new sequence.
    int addToPaperEdit();
    Id assemblePaperEdit(const QString& name = QString());
    std::vector<PaperLine> paperEdit() const;  // in the list's order
    void clearPaperEdit();

signals:
    void sourceSeekRequested(montage::FrameTime frame);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void rebuild();
    void refreshHighlights();
    int wordAtPosition(int pos) const;
    double fps() const;
    // Frames of Smooth Cut to put at the joins a deletion leaves (0 when off).
    FrameTime smoothCutFrames() const;

    struct Span {
        int pos = 0, len = 0;
        int word = -1;  // -1: a timecode label (seeks to `seconds`)
        double seconds = 0;
    };

    EditorState* state_;
    Mode mode_ = Mode::Sequence;
    QComboBox* modeBox_;
    QLineEdit* search_;
    QLabel* matches_;
    QTextEdit* text_;
    QLabel* status_;
    QToolButton* deleteBtn_;
    QToolButton* fillersBtn_;
    QToolButton* retakesBtn_;
    QToolButton* pausesBtn_;
    QToolButton* bleepBtn_;
    QToolButton* insertBtn_;
    QToolButton* overwriteBtn_;
    QToolButton* smoothBtn_;
    QToolButton* paperAddBtn_;
    QToolButton* paperBuildBtn_;
    QListWidget* paperList_;
    std::vector<TranscriptWord> words_;
    std::vector<Span> spans_;
    QString signature_;
    std::vector<std::pair<int, int>> found_;  // word ranges matching the search
    int current_ = -1;                        // word under the playhead
    FrameTime sourcePosition_ = 0;
};

}  // namespace montage
