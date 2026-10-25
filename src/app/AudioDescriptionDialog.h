// Montage — Sequence › Audio Description: the gaps between the dialogue where a description fits, the descriptions
// written into them (each with how well it fits, at a describer's pace), voiced by the speech generator onto an AD
// track as clips of the role "Description", the programme ducked under them, and whether they are heard while
// working (core/AudioDescription.h). Drafts can be written from the pictures on this computer. Exports add the
// described stream (Export › Audio description).
#pragma once

#include <QDialog>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/AudioDescription.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace montage {

class EditorState;

class AudioDescriptionDialog : public QDialog {
    Q_OBJECT
public:
    AudioDescriptionDialog(EditorState* state, QWidget* parent = nullptr);
    ~AudioDescriptionDialog() override;

    // Finds the gaps in the dialogue (the audio tracks other than the AD track) and lists them with the descriptions.
    int findGaps(QString* error = nullptr);
    // A description from the playhead: to the next line of dialogue, at most three seconds.
    void addAtPlayhead();
    // Drafts a description for each gap that has none, from the picture in its middle (render/FootageWords.h): put into
    // words, and written as a describer would by the language model when "Write with the language model" is on.
    // Returns the descriptions drafted (-1 on failure, with `error`).
    int draft(QString* error = nullptr);
    // Voices the descriptions onto the AD track (replacing what was voiced before). Returns the clips made.
    int voice(QString* error = nullptr);
    // Ducks every other clip under the voiced descriptions. Returns how many clips changed.
    int duck();
    // Whether descriptions are heard while working (they always go to the described stream of an export).
    void setHear(bool on);
    int rows() const;

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void refresh();
    // Whether the gaps (or those added at the playhead, when none were found) were looked for before the dialogue was
    // last cut.
    bool stale() const;
    // Drops (or shortens) the gaps added at the playhead that the dialogue now covers; with `alone`, when no gaps were
    // found, they are all there is. Returns false (with `error`) when the dialogue cannot be heard.
    bool checkAdded(QString* error = nullptr);
    void keepAdded(const std::vector<std::pair<double, double>>& speech, double fps);
    // checkAdded's and findGaps's second halves: the dialogue's speech (or why it could not be heard) used, as it was
    // with the dialogue's clips at `layout`.
    bool useAdded(const std::vector<std::pair<double, double>>& speech, const std::string& err, const std::string& layout,
                  QString* error);
    int useFound(const std::vector<std::pair<double, double>>& speech, const std::string& err, const std::string& layout,
                 QString* error);
    void cellEdited(int row, int column);
    int adTrack() const;  // the audio track named "AD", -1 if none
    std::string dialogueLayout() const;  // where the dialogue's clips are (the AD track aside), to tell when it is cut

    EditorState* state_;
    QTableWidget* table_;
    QDoubleSpinBox* minGap_;
    QSpinBox* pace_;
    QComboBox* voice_;
    QDoubleSpinBox* duckDb_;
    QCheckBox* hear_;
    QCheckBox* useModel_;
    QLabel* status_;
    std::vector<DescriptionGap> gaps_;
    std::vector<DescriptionGap> added_;  // the gaps added at the playhead, kept when the gaps are found again
    std::string foundFor_;               // the dialogue's layout when the gaps were found
    std::string addedFor_;               // and when the gaps added were checked, when none were found
    bool refreshing_ = false;
    QTimer* refind_;  // finds the gaps again after the dialogue is cut (restarted by each edit)
    bool listening_ = false, listenAgain_ = false;  // the dialogue being listened to off the UI thread, and again after
    // Stops that listening when the dialog is hidden or goes (it is started again on show).
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace montage
