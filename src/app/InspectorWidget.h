// Montage — inspector ("Effect Controls"): clip properties, fixed
// attributes, effect stacks and transitions, with keyframes on every parameter.
#pragma once

#include <QScrollArea>
#include <array>
#include <functional>
#include <vector>

#include "core/Effects.h"
#include "core/Model.h"

class QVBoxLayout;
class QFormLayout;

namespace montage {

class EditorState;
class ColorWheel;

class InspectorWidget : public QScrollArea {
    Q_OBJECT
public:
    explicit InspectorWidget(EditorState* state, QWidget* parent = nullptr);

private:
    // How a parameter row finds its Effect in a (mutable) sequence and which
    // frame keyframes are read / written at.
    struct Target {
        std::function<Effect*(Sequence&)> resolve;
        std::function<FrameTime()> time;    // clip-relative frame
        std::function<FrameTime()> origin;  // timeline frame of the clip start (for keyframe navigation)
        QString key;                        // merge-key prefix
        bool keyframes = true;
        std::function<void(Sequence&)> afterWrite;  // runs in the same edit (e.g. to keep linked clips in step)
        Id clip = 0;  // a clip's own effect: value changes reach the same effect on the other selected clips
        Id effect = 0;
        Id owner = 0;  // the clip, track, bus or sequence whose chain holds the effect
    };
    // The other selected clips a change to `target` also goes to (Resolve's multi-clip Inspector), and applying
    // `fn` to their matching effects at their own times, inside an edit.
    std::vector<Id> otherSelected(const Target& target) const;
    void applyToOthers(Sequence& s, const Target& target, const std::vector<Id>& others, FrameTime playhead,
                       const std::function<void(Effect&, FrameTime)>& fn) const;

    void rebuild();
    void refreshValues();
    QString signature() const;
    void buildClip(const Clip& c, TrackKind kind);
    void buildTransition(const Transition& t, TrackKind kind);
    QFormLayout* addSection(const QString& title, QWidget* headerExtra = nullptr, bool startCollapsed = false);
    void addParamRows(QFormLayout* form, const EffectInfo& info, const Target& target);
    // A colour wheel over three channel controls of `target` (rim = `scale`, centre = `neutral`).
    ColorWheel* addWheel(QWidget* parent, const QString& title, const std::array<std::string, 3>& names, double scale, double neutral,
                         const Target& target);
    // Lift, Gamma and Gain wheels over Color Correct's per-channel controls.
    void addColorWheels(QFormLayout* form, const Target& target);
    // The HDR Palette: a zone picker, the zone's wheel and its sliders.
    void addHdrPalette(QFormLayout* form, const EffectInfo& info, const Target& target);
    void addParamRow(QFormLayout* form, const ParamInfo& pi, const Target& target);
    void addStringRow(QFormLayout* form, const StringParamInfo& si, const Target& target);
    // The effect stack of a clip, audio track, bus or master (by owner id), with its Add menu.
    void buildEffectStack(Id owner, TrackKind kind, const std::vector<Effect>& effects,
                          const std::function<FrameTime()>& localTime);
    void buildChain(Id owner);
    void addEffectMenu(TrackKind kind, Id owner);

public:
    // Analyses clip `clip`'s footage for its Stabilize effect `effect`.
    void analyzeStabilize(Id clip, Id effect);
    // Tracks the mask of `effect` on `clip` from the playhead to the clip's end
    // (forward) or start; model 0 position, 1 + scale, 2 + rotation.
    void trackMask(Id clip, Id effect, bool forward, int model);
    // Follows the object of `effect`'s Object mask from the playhead to the clip's end (or start).
    void trackObject(Id clip, Id effect, bool forward);
    // Follows the surface under the corners of Corner Pin `effect` on `clip`
    // (in the clip beneath it, or its own footage) from the playhead to the
    // clip's end (forward) or start, keying the corners.
    void trackCorners(Id clip, Id effect, bool forward);
    // Moves `clip` with what is under its position in the footage beneath, from
    // the playhead to the clip's end (forward) or start; model 0 position,
    // 1 + scale, 2 + rotation; `size` the tracked square, a fraction of the frame height.
    void followFootage(Id clip, bool forward, int model, double size);

private:
    // Runs `work` off the UI thread behind a progress dialog with Cancel;
    // false (with the error shown) if it fails or is cancelled.
    bool runAnalysis(const QString& title,
                     const std::function<bool(const std::function<void(double)>&, const std::atomic<bool>*, std::string*)>& work);

    EditorState* state_;
    QWidget* content_ = nullptr;
    QVBoxLayout* layout_ = nullptr;
    QString signature_;
    std::vector<std::function<void()>> refreshers_;
    int hdrZone_ = 0;  // the HDR Palette zone shown (0 Global, then Black to Specular)
};

}  // namespace montage
