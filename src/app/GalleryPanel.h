// Montage — the Gallery panel (Resolve's Gallery): stills of the Program monitor with their clips' grades, in albums
// (render/Gallery.h). Grab Still takes one at the playhead; Wipe compares the picture with a still in the Program
// monitor; Apply Grade puts a still's grade on the selected clips (one undo step).
#pragma once

#include <QImage>
#include <QWidget>

#include "core/Model.h"

class QComboBox;
class QListWidget;
class QPushButton;

namespace montage {

class EditorState;
struct GalleryStill;

class GalleryPanel : public QWidget {
    Q_OBJECT
public:
    explicit GalleryPanel(EditorState* state, QWidget* parent = nullptr);

    QString folder() const;
    QString album() const;
    void setAlbum(const QString& name);
    // Reads the album again (another window or MCP may have changed it).
    void refresh();
    // The edits; false (with a message) when they cannot be made.
    bool grab();
    bool applyToSelection();
    bool wipe(bool on);
    bool removeSelected();
    bool addAlbum(const QString& name);
    // The still picked (its id), or "".
    QString selectedStill() const;
    void selectStill(const QString& id);
    // Turns Wipe off without asking the viewer (another comparison took its place).
    void wipeEnded();

signals:
    void compareRequested(const QImage& still, const QString& label);
    void compareCleared();

private:
    bool still(const QString& id, GalleryStill& out) const;

    EditorState* state_;
    QComboBox* albums_;
    QListWidget* list_;
    QPushButton* wipe_;
    QString wiping_;  // the still being wiped against
    QString shownFolder_;
    uint64_t shownProject_ = 0;  // EditorState::projectGeneration when it was shown
};

}  // namespace montage
