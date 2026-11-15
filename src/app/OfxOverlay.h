// Montage — OpenFX plugins' own on-screen controls on the Program monitor (render/Ofx.h Interact): for the selected
// clip under the playhead, each enabled OpenFX effect (or an OpenFX generator) with an overlay draws over the picture,
// and the pen and keys go to it. What a drag sets is written as one undo step, keyed where the parameter is animated.
// With plug-ins isolated the controls run in the plug-in's host, which draws them; ones not answering in time keep
// their last picture up, marked as not responding, and ones that fail are closed, their plug-in named.
#pragma once

#include <QObject>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"
#include "render/Ofx.h"

class QPainter;
class QRectF;
class QTimer;

namespace montage {

class EditorState;
class ViewerWidget;

class OfxOverlay : public QObject {
    Q_OBJECT
public:
    OfxOverlay(EditorState* state, ViewerWidget* viewer);
    ~OfxOverlay() override;

    // The effects showing controls now: (clip, effect), the generator's id for an OpenFX generator.
    struct Target {
        Id clip = 0;
        Id effect = 0;
        bool generator = false;
        FrameTime lt = 0;  // the clip frame at the playhead
    };
    std::vector<Target> targets() const;
    // The view (the monitor widget's pixels) for a clip's frame at the playhead; false when it has no picture.
    bool view(Id clip, ofx::View& out) const;
    // Whether an effect's open controls run in a plug-in host, and whether they are not answering it now.
    bool inHost(Id clip, Id effect) const;
    bool unresponsive(Id clip, Id effect) const;
    // What an answer that came late may still write. `asked`: the effect as it was when the control was sent the pen
    // or key the answer is to; `now`: as it is; `changes`: the parameters and strings the answer sets. `edited` is
    // `now` with those of them that are still as they were when asked (one changed since keeps its newer value);
    // false when that leaves nothing to change.
    static bool lateAnswer(const Effect& asked, const Effect& now, const Effect& changes, Effect& edited);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void paint(QPainter& p, const QRectF& picture);
    const Effect* effectOf(const Target& t) const;
    ofx::Interact* interactFor(const Target& t, const Effect& e);
    // Writes what the plugin set into the effect (inside the open gesture, or as its own undo step).
    void apply(const Target& t, const Effect& edited, bool gesture);
    // What a late answer from controls in a host set (`changes`: those parameters and strings), as its own step:
    // lateAnswer's, against the effect as it was when the control was asked (`asked`).
    void applyLate(const Target& t, const Effect& changes, const Effect& asked);
    // The effect as it was when its control was last sent a pen or key with no answer out (`now` when never).
    Effect askedAs(const Target& t, const Effect& now) const;
    void noteAsked(const Target& t, const Effect& e, const ofx::Interact& in);
    bool pen(ofx::Interact::Pen kind, QEvent* e);

    EditorState* state_;
    ViewerWidget* viewer_;
    struct Open {
        std::string plugin;  // ofx_id it was opened for
        Target target;
        std::unique_ptr<ofx::Interact> interact;
        Effect asked;  // the effect when a pen or key was last sent with no answer out
        bool hasAsked = false;
    };
    std::map<std::pair<Id, Id>, Open> open_;
    Target pressed_;           // the effect a press went to, while the pen is down
    bool down_ = false;
    bool changed_ = false;     // the press's drag set something
    QTimer* poll_ = nullptr;
};

}  // namespace montage
