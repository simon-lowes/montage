// Montage — inspector ("Effect Controls"): clip properties, fixed
// attributes, effect stacks and transitions, with keyframes on every parameter.
#pragma once

#include <QScrollArea>
#include <array>
#include <functional>
#include <vector>

#include "core/Effects.h"
#include "core/Model.h"
#include "render/FollowFocus.h"

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
        Id publishClip = 0;  // a picture clip of the open sequence: its settings can be published to the graphic it is part of
    };
    // The other selected clips a change to `target` also goes to (Resolve's multi-clip Inspector), and applying
    // `fn` to their matching effects at their own times, inside an edit.
    std::vector<Id> otherSelected(const Target& target) const;
    // Whether `target` is one of its clip's own settings an expression can drive (its Transform, generator, sound or an
    // effect it carries; not its speed, a trim, a grade node or a group's).
    bool ownSetting(const Target& target) const;
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
    void addColorSlice(QFormLayout* form, const EffectInfo& info, const Target& target);
    void addParamRow(QFormLayout* form, const ParamInfo& pi, const Target& target);
    void addStringRow(QFormLayout* form, const StringParamInfo& si, const Target& target);
    // The effect stack of a clip, audio track, bus or master (by owner id), with its Add menu.
    // `node`: only the effects in that colour node (0: those outside the clip's node graph; -1 all).
    // `addMenu`: with its Add menu.
    void buildEffectStack(Id owner, TrackKind kind, const std::vector<Effect>& effects,
                          const std::function<FrameTime()>& localTime, int node = -1, bool addMenu = true);
    // A clip's colour nodes (core/GradeGraph.h): the picked one, or each in turn, then the effects outside them.
    void buildGradeNodes(const Clip& clip, const std::function<FrameTime()>& localTime);
    void buildChain(Id owner);
    void addEffectMenu(TrackKind kind, Id owner, int node = 0);

    // Publishes a clip's setting as a field of the graphic its sequence is, asking for its label.
    void publishParameter(const Target& target, const std::string& param, const QString& label);
    // The expression on a clip's setting (core/Expression.h): written, tried as it is typed, removed or baked to keys.
    void editExpression(const Target& target, const std::string& param, const QString& label, double def);
    // A graphic's own values for the fields its template publishes (core/GraphicsTemplate.h).
    void addGraphicSection(const Clip& clip);
    // An OGraf graphic's data, a row per field of its schema, and rendering it ahead.
    void addOGrafFields(QFormLayout* form, const Clip& clip, const Target& target);
    void renderOGrafClip(Id clip);

public:
    // Publishes setting `param` of effect `effect` (0: the generator) on clip `clip` of the open sequence as a field
    // labelled `label`; "" or why not.
    QString publishField(Id clip, Id effect, const std::string& param, const QString& label);
    // Analyses clip `clip`'s footage for its Stabilize effect `effect`.
    void analyzeStabilize(Id clip, Id effect);
    // Solves the camera of clip `clip`'s footage into its 3D Camera Tracker `effect`.
    void solveCameraTrack(Id clip, Id effect);
    // A title saying `text` placed in the scene of `clip`'s solve, where its 3D Camera Tracker `effect`'s settings say
    // at the playhead; its id, 0 (with the reason shown) when it cannot be.
    Id placeTitleInScene(Id clip, Id effect, const QString& text);
    // Tracks the mask of `effect` on `clip` from the playhead to the clip's end
    // (forward) or start; model 0 position, 1 + scale, 2 + rotation.
    void trackMask(Id clip, Id effect, bool forward, int model);
    // Follows the object of `effect`'s Object mask from the playhead to the clip's end (or start).
    void trackObject(Id clip, Id effect, bool forward);
    // Analyses the depth over `clip` (media/VideoDepth.h), so its depth effects hold still.
    void analyzeDepth(Id clip);
    // Keys Lens Blur (Depth) `effect`'s Focus Depth to follow `target` (render/FollowFocus.h): its Focus Point at the
    // playhead, its Object mask, or the face nearest the point.
    void followFocusOn(Id clip, Id effect, FocusTarget target);
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
    int sliceVector_ = 0;  // the Hue Vectors vector shown (0 Global, then Red to Magenta, then Skin)
    int hdrZone_ = 0;  // the HDR Palette zone shown (0 Global, then Black to Specular)
};

}  // namespace montage
