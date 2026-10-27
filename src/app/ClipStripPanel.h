// Montage — the colour page's clip strip and lightbox (Resolve's thumbnail timeline and Lightbox, Premiere's Colour
// Mode clip grid): every picture clip of the sequence in timeline order as a graded thumbnail (its own track alone, at
// its middle frame), numbered, with its timecode and name. Clicking one selects it and parks on it; the clip under the
// playhead is marked. Lightbox shows them as a grid of larger thumbnails to judge a scene's grades together. Groups
// shows the colour groups as rows (core/ColorGroups.h: scene groups holding angle groups holding clips): Group Shots
// makes them (render/ShotGroups.h) at the sensitivity set beside it; rows can be split, joined, locked and dragged into.
#pragma once

#include <QHash>
#include <QImage>
#include <QWidget>
#include <atomic>
#include <memory>

#include "core/Model.h"

class QListWidget;
class QSlider;
class QToolButton;
class QTreeWidget;
class QWidget;

namespace montage {

class EditorState;

class ClipStripPanel : public QWidget {
    Q_OBJECT
public:
    explicit ClipStripPanel(EditorState* state, QWidget* parent = nullptr);

    void setLightbox(bool on);
    bool lightbox() const;
    // The clips shown, in order.
    std::vector<Id> clips() const;
    // Thumbnails still being drawn.
    int pending() const { return pending_; }
    // Lists the clips again now (it also does so shortly after each change).
    void rebuild();

    // The colour groups as rows.
    void setGroupsShown(bool on);
    bool groupsShown() const;
    QTreeWidget* groupTree() const { return tree_; }
    double sensitivity() const;
    // On the rows selected: the clips split off into a group of their own, the groups joined into the first, the
    // groups locked or unlocked. Whether anything changed.
    bool splitSelected();
    bool joinSelected();
    bool lockSelected(bool locked);
    // Clips or groups dragged onto a group's row (0: onto Not Grouped, out of their groups).
    bool moveInto(const std::vector<Id>& items, Id group);

signals:
    void groupShotsRequested(double sensitivity);

private:
    void requestThumbnails();
    void markCurrent();
    void rebuildGroups();
    std::vector<Id> selectedRows(int kind) const;  // the ids of the selected rows of a kind (0 clip, 1 group)

    EditorState* state_;
    QListWidget* list_;
    QToolButton* lightbox_;
    QToolButton* groups_;
    QWidget* groupBar_;
    QSlider* sensitivity_;
    QTreeWidget* tree_;
    QHash<QString, QImage> thumbs_;  // by what the thumbnail shows (the clip as saved, its track, the sequence's size)
    QHash<Id, QString> keys_;        // each clip's, as listed
    int pending_ = 0;
    std::shared_ptr<std::atomic<int>> generation_ = std::make_shared<std::atomic<int>>(0);  // (read by the renders)
};

}  // namespace montage
