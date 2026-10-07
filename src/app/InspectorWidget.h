// Montage — inspector ("Effect Controls"): clip properties, fixed
// attributes, effect stacks and transitions, with keyframes on every parameter.
#pragma once

#include <QScrollArea>
#include <functional>
#include <vector>

#include "core/Effects.h"
#include "core/Model.h"

class QVBoxLayout;
class QFormLayout;

namespace montage {

class EditorState;

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
    };

    void rebuild();
    void refreshValues();
    QString signature() const;
    void buildClip(const Clip& c, TrackKind kind);
    void buildTransition(const Transition& t, TrackKind kind);
    QFormLayout* addSection(const QString& title, QWidget* headerExtra = nullptr, bool startCollapsed = false);
    void addParamRows(QFormLayout* form, const EffectInfo& info, const Target& target);
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
};

}  // namespace montage
