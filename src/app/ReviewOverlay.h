// Montage — reviewers' drawings over the Program monitor (Resolve's and Frame.io's annotations): a marker's drawing
// (core/ReviewPage.h, from a review page's notes) over the picture while the playhead is on its frame, or anywhere in
// its range for a marker with a length. Playback › Show Review Drawings turns them off.
#pragma once

#include <QObject>
#include <vector>

#include "core/ReviewPage.h"

class QPainter;
class QRectF;

namespace montage {

class EditorState;
class ViewerWidget;

class ReviewOverlay : public QObject {
    Q_OBJECT
public:
    ReviewOverlay(EditorState* state, ViewerWidget* viewer);
    void setShown(bool on);
    bool shown() const { return shown_; }
    // What is drawn at the playhead now (every marker's there, in order).
    std::vector<ReviewStroke> strokesNow() const;
    // Strokes over `picture` (the rectangle the frame is drawn in), as the review page draws them.
    static void paintStrokes(QPainter& p, const QRectF& picture, const std::vector<ReviewStroke>& strokes);

private:
    EditorState* state_;
    ViewerWidget* viewer_;
    bool shown_ = true;
};

}  // namespace montage
