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
    void addEffectMenu(TrackKind kind, Id clip);

    EditorState* state_;
    QWidget* content_ = nullptr;
    QVBoxLayout* layout_ = nullptr;
    QString signature_;
    std::vector<std::function<void()>> refreshers_;
};

}  // namespace montage
