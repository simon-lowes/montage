// Montage — caption tracks of the active sequence: generate them from
// transcripts, import or export them (SubRip, WebVTT, SCC, TTML, EBU STL,
// ASS), edit the text and timing of each caption, check them against reading
// limits and fix, shift, sync or search them as a whole, and style how they
// are drawn.
#pragma once

#include <QWidget>

#include "core/CaptionTools.h"
#include "core/Captions.h"
#include "core/Model.h"

class QCheckBox;
class QLabel;
class QMenu;
class QComboBox;
class QTableWidget;
class QTableWidgetItem;
class QToolButton;

namespace montage {

class EditorState;

class CaptionsPanel : public QWidget {
    Q_OBJECT
public:
    explicit CaptionsPanel(EditorState* state, QWidget* parent = nullptr);

    // The track shown in the panel (0 if the sequence has none).
    Id currentTrack() const { return track_; }
    void setCurrentTrack(Id id);
    // Replaces the current track's captions with ones built from transcripts
    // (or adds a track when there is none). Returns the number of captions.
    int generateFromTranscripts(const CaptionRules& rules = {});
    bool importFile(const QString& path, QString* error = nullptr);
    bool exportFile(const QString& path, QString* error = nullptr) const;
    // Shows the caption and starts editing its text.
    void editCaption(Id track, int index);
    // The spelling menu for caption `row` (a submenu per misspelt word: suggestions, Add to Dictionary), or null.
    QMenu* spellingMenu(int row);
    // A translated copy of the current track, as a new track (one undo step),
    // after downloading the models it needs if asked. Returns its id, or 0.
    Id translateTrack(const std::string& to, QString* error = nullptr);
    // Dubs the current track into English: translated first unless it is
    // English already, spoken cue by cue at each cue's time with an AI voice
    // (Kokoro) on a new audio track, and every other audio clip lowered by
    // `duckDb` under the speech (0: left as it is) so the original stays
    // faintly audible, as in a voice-over translation.
    struct DubResult {
        Id captions = 0;      // the English track
        int audioTrack = -1;  // the dub's audio track
        std::vector<Id> clips;
        int ducked = 0;       // clips lowered under it
    };
    DubResult dub(const std::string& voice, double duckDb = -18, QString* error = nullptr);

    // Subtitle tools. The reading limits (remembered between sessions) and what each caption of the current
    // track breaks; the selected captions (rows), or all of them when none are.
    CaptionLimits limits() const;
    void setLimits(const CaptionLimits& limits);
    std::vector<unsigned> issues() const;
    std::vector<size_t> selectedCaptions() const;
    // Each one undo step on the current track: Fix Timing (how many changed), shifting the selected captions (or
    // all), syncing so the first and last of the selection (or the track) start at `first` and `last`, and find and
    // replace in the selected captions (or all; how many replaced).
    int fixTiming();
    bool shiftCaptions(FrameTime delta);
    bool syncToTwoPoints(FrameTime first, FrameTime last);
    int findReplace(const QString& find, const QString& replace, bool caseSensitive, bool wholeWords);
    // Moves the selected captions (else the one under the playhead) up or down (`vertical`, -1 to
    // keep) and lines them up (`align`, -1 to keep); one undo step. False if none moved.
    bool placeCaptions(int vertical, int align);
    // Moves captions shown over low titles (lower thirds, crawls) to the top; how many moved.
    int raiseOverTitles();
    // Caption looks: the built-in ones (by id) and the editor's own saved ones (by name, remembered between sessions).
    // Applying one restyles the current track in one undo step; saving keeps the current track's style under a name.
    QStringList savedLooks() const;
    bool applyLook(const QString& idOrName);
    bool saveLook(const QString& name);

public slots:
    void generateDialog();
    void importDialog();
    void exportDialog();
    void styleDialog();
    void addTrack();
    void translateDialog();
    void dubDialog();
    void shiftDialog();
    void syncDialog();
    void findReplaceDialog();
    void limitsDialog();

private:
    void rebuild();
    void followPlayhead(FrameTime t);
    const CaptionTrack* track() const;
    // Runs an undoable edit on the current track.
    bool editTrack(const QString& label, const std::function<bool(CaptionTrack&)>& fn, const QString& mergeKey = {});
    void itemChanged(QTableWidgetItem* item);
    void addAtPlayhead();
    void deleteSelected();
    void mergeWithNext();
    void splitAtPlayhead();

    EditorState* state_;
    Id track_ = 0;
    QComboBox* tracks_;
    QCheckBox* visible_;
    QTableWidget* table_;
    QLabel* summary_ = nullptr;  // how many captions break the reading limits
    bool rebuilding_ = false;
};

}  // namespace montage
