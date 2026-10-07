// Montage — Find Shots: search the project's footage by what it shows. Type
// a description ("a dog on a beach", "close-up of hands"); the best-matching
// moments are listed with a thumbnail, and opening one loads it into the
// Source monitor with In and Out around it. Videos are indexed once (CLIP,
// media/VisualSearch.h) and the index is saved with the project.
#pragma once

#include <QWidget>
#include <vector>

#include "core/Model.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace montage {

class EditorState;
class ThumbnailCache;

// Indexes the given videos (a subclip's media, for a subclip) that have no
// visual index yet, offering the model download first and showing progress.
// False if cancelled or failed.
bool indexVideos(EditorState* state, const std::vector<Id>& media, QWidget* parent);

class ShotSearchPanel : public QWidget {
    Q_OBJECT
public:
    explicit ShotSearchPanel(EditorState* state, QWidget* parent = nullptr);

    // Searches (indexing unindexed videos first, after asking). Returns the number of moments found.
    int search(const QString& query);
    // Indexes the project's videos that have no index yet; false if cancelled or failed.
    bool indexMissing();
    const std::vector<ShotMatch>& results() const { return results_; }
    // Opens result i in the Source monitor.
    void open(int i);
    // Saves result i as a subclip named after the search; returns its id.
    Id makeSubclip(int i);

signals:
    void openRequested(montage::Id media, montage::FrameTime in, montage::FrameTime out, montage::FrameTime at);

private:
    void refreshStatus();
    void showResults();

    EditorState* state_;
    QLineEdit* query_;
    QPushButton* searchBtn_;
    QPushButton* indexBtn_;
    QLabel* status_;
    QListWidget* list_;
    ThumbnailCache* thumbs_;
    std::vector<ShotMatch> results_;
    QString lastQuery_;
};

}  // namespace montage
