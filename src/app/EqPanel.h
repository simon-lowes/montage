// Montage — the Parametric EQ's editor: its display (app/EqDisplay.h), with above it the range shown, the spectrum
// switch, Add Band and the large window, and docked below it a strip of fine controls for the band picked (type,
// frequency, gain, Q, slope, on, a Solo held to hear that band alone), then the tips. In the Inspector, and large in a
// window of its own; the two stay in step through the project, and share which band is picked.
//
// A drag is one undo step; so are a run of wheel turns, key presses or strip changes to one band. Solo changes only
// what playback hears while it is held (EditorState::setAudition), never the project.
#pragma once

#include <QPointer>
#include <QWidget>
#include <functional>
#include <string>

#include "core/Model.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QToolButton;

namespace montage {

class EditorState;
class EqDisplay;

class EqPanel : public QWidget {
    Q_OBJECT
public:
    // The EQ `effect` in the chain of `owner` (a clip, track, bus or the sequence); `time` its frame now (where keys are
    // read and set). `large`: in its own window.
    EqPanel(EditorState* state, Id owner, Id effect, std::function<FrameTime()> time, bool large, QWidget* parent = nullptr);
    ~EqPanel() override;

    EqDisplay* display() const { return display_; }
    Id effectId() const { return effect_; }
    // Opens the EQ large in a resizable window of its own (raises it when it is open); the window.
    static QWidget* openWindow(EditorState* state, Id owner, Id effect, std::function<FrameTime()> time, QWidget* parent);
    static QWidget* openWindowFor(Id effect);  // the large window open for that EQ, or null
    // The setting that says the tips were hidden ("Hide tips"); "?" shows them again.
    static QString tipsHiddenKey() { return QStringLiteral("eq/tipsHidden"); }
    bool tipsShown() const;

private:
    void build();
    void refresh();
    // The band picked, shared by every panel on this EQ.
    void pick(const std::string& band);
    // Writes a band's values at the frame now, as one undo step with the other changes of `mergeKey` that follow.
    void write(const std::string& band, const std::vector<std::pair<std::string, double>>& values, const QString& label,
               const QString& mergeKey);
    // An edit that adds or takes away params (a band, a type): made once the event that asked for it is over, as the
    // Inspector builds itself again for it.
    void reshape(const QString& label, std::function<std::string(Effect&)> fn);
    void solo(const std::string& band, bool held);
    void showTips(bool show);

    EditorState* state_;
    Id owner_, effect_;
    std::function<FrameTime()> time_;
    bool large_;
    EqDisplay* display_ = nullptr;
    QComboBox* range_ = nullptr;
    QCheckBox* spectrum_ = nullptr;
    QToolButton* addBand_ = nullptr;
    QPushButton* openLarge_ = nullptr;
    QToolButton* help_ = nullptr;
    // the band strip
    QWidget* strip_ = nullptr;
    QLabel* bandName_ = nullptr;
    QComboBox* type_ = nullptr;
    QComboBox* slope_ = nullptr;
    QDoubleSpinBox* hz_ = nullptr;
    QDoubleSpinBox* db_ = nullptr;
    QDoubleSpinBox* q_ = nullptr;
    QCheckBox* on_ = nullptr;
    QPushButton* solo_ = nullptr;
    QToolButton* remove_ = nullptr;
    // the tips
    QWidget* tips_ = nullptr;
    QWidget* tipsBody_ = nullptr;
    QToolButton* tipsToggle_ = nullptr;
    std::string soloing_;  // the band held soloed ("" none)
    bool dragging_ = false;
    double pressHz_ = 0, pressDb_ = 0;  // the dot dragged, where it was taken
};

}  // namespace montage
