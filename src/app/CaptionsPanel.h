// Montage — caption tracks of the active sequence: generate them from
// transcripts, import or export SubRip / WebVTT / SCC, edit the text and
// timing of each caption, and style how they are drawn.
#pragma once

#include <QWidget>

#include "core/Captions.h"
#include "core/Model.h"

class QCheckBox;
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

public slots:
    void generateDialog();
    void importDialog();
    void exportDialog();
    void styleDialog();
    void addTrack();
    void translateDialog();
    void dubDialog();

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
    bool rebuilding_ = false;
};

}  // namespace montage
