// Montage — the colour page's clip strip and lightbox (Resolve's thumbnail timeline and Lightbox, Premiere's Colour
// Mode clip grid): every picture clip of the sequence in timeline order as a graded thumbnail (its own track alone, at
// its middle frame), numbered, with its timecode and name. Clicking one selects it and parks on it; the clip under the
// playhead is marked. Lightbox shows them as a grid of larger thumbnails to judge a scene's grades together.
#pragma once

#include <QHash>
#include <QImage>
#include <QWidget>
#include <memory>

#include "core/Model.h"

class QListWidget;
class QToolButton;

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

private:
    void requestThumbnails();
    void markCurrent();

    EditorState* state_;
    QListWidget* list_;
    QToolButton* lightbox_;
    QHash<QString, QImage> thumbs_;  // by what the thumbnail shows (the clip as saved, its track, the sequence's size)
    QHash<Id, QString> keys_;        // each clip's, as listed
    int pending_ = 0;
    std::shared_ptr<int> generation_ = std::make_shared<int>(0);
};

}  // namespace montage
