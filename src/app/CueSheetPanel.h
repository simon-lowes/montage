// Montage — the Cue Sheet panel (Window › Cue Sheet): the music cue sheet of the active sequence (core/Paperwork.h),
// a row per cue, timed in, out and by duration. Its title, composer, publisher, society, library, catalogue number,
// ISRC and ISWC come from the file's tags until typed over; what is typed, and each cue's use (background or visual,
// vocal or instrumental, main or end title), is kept with its media. Suggest Vocal asks the vocal stem; Export writes
// the sheet as XLSX, CSV or PDF (File › Export Paperwork).
#pragma once

#include <QWidget>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Paperwork.h"

class QLabel;
class QPushButton;
class QTableWidget;

namespace montage {

class EditorState;

class CueSheetPanel : public QWidget {
    Q_OBJECT
public:
    explicit CueSheetPanel(EditorState* state, QWidget* parent = nullptr);

    void refresh();
    const std::vector<MusicCue>& cues() const { return cues_; }
    // Marks each cue not yet called vocal or instrumental, from its vocal stem (off the UI thread). False with a
    // reason in the status bar when the stem model is missing.
    bool suggestVocal();

signals:
    void exportRequested();

protected:
    void showEvent(QShowEvent* e) override;

private:
    void edited(int row, int column);
    void setField(Id media, const std::string& key, const std::optional<std::string>& value);  // none: the media's word taken away
    std::map<std::string, std::string> tagsOf(const MediaItem& m);

    EditorState* state_;
    QTableWidget* table_;
    QLabel* summary_;
    QPushButton* suggest_;
    QPushButton* export_;
    std::vector<MusicCue> cues_;
    std::map<std::string, std::map<std::string, std::string>> tags_;  // by file, read once
    bool refreshing_ = false;
    bool stale_ = true;
    bool suggesting_ = false;
};

}  // namespace montage
