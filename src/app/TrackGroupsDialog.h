// Montage — Sequence › Track Groups…: named groups of tracks edited, mixed and comped together (core/TrackGroups.h),
// each with its switches and suspension, the tracks in it, and each track's label colour. Every change is its own undo
// step and shows at once on the timeline and in the mixer; the dialog follows edits made elsewhere while it is open.
#pragma once

#include <QDialog>

#include "core/Model.h"

class QCheckBox;
class QComboBox;
class QListWidget;
class QPushButton;

namespace montage {

class EditorState;

class TrackGroupsDialog : public QDialog {
    Q_OBJECT
public:
    TrackGroupsDialog(EditorState* state, QWidget* parent = nullptr);

    QListWidget* groups() const { return groups_; }
    QListWidget* tracks() const { return tracks_; }
    // A switch's box by its object name: groupEditing, groupMute, groupVolume, groupPan, groupAutomation, groupRecord,
    // groupComp, groupSuspended.
    QCheckBox* switchBox(const QString& name) const;
    QComboBox* colorBox() const { return color_; }
    QCheckBox* colorClipsBox() const { return colorClips_; }
    // Makes a group of that name (no prompt) and shows it; false if it could not be made.
    bool newGroup(const QString& name);

private:
    void refresh();
    Id currentGroup() const;
    TrackRef currentTrack(bool* ok) const;
    void promptNewGroup();
    void renameGroup();
    void deleteGroup();
    void switchesChanged();
    void memberToggled(int row);
    void colorChosen(int index);

    EditorState* state_;
    QListWidget* groups_ = nullptr;
    QListWidget* tracks_ = nullptr;
    QPushButton* rename_ = nullptr;
    QPushButton* remove_ = nullptr;
    std::vector<QCheckBox*> switches_;
    QComboBox* color_ = nullptr;
    QCheckBox* colorClips_ = nullptr;
    bool updating_ = false;
};

}  // namespace montage
