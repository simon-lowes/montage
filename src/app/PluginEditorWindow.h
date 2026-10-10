// Montage — a plugin's own editor in a window. The editor runs on its own
// instance of the plugin: knob turns become ordinary (undoable, keyframe
// aware) parameter edits of the clip effect, its settings are saved into the
// effect, and the playing instance picks both up.
#pragma once

#include <QWidget>
#include <map>
#include <memory>

#include "audio/Plugins.h"
#include "core/Model.h"

class QTimer;

namespace montage {

class EditorState;

class PluginEditorWindow : public QWidget, public plugins::EditorListener {
    Q_OBJECT
public:
    // Opens the editor of plugin effect `effect` in the chain owned by `clip`
    // (a clip, audio track, bus or the sequence for master), or raises it
    // if it is already open. nullptr (and `error`) if the plugin has none.
    static PluginEditorWindow* open(EditorState* state, Id clip, Id effect, QWidget* parent, QString* error = nullptr);
    static PluginEditorWindow* find(Id clip, Id effect);
    ~PluginEditorWindow() override;

    Id clipId() const { return clip_; }
    Id effectId() const { return effect_; }
    plugins::Instance* instance() const { return inst_.get(); }
    // Stores the editor's current settings in the effect (also done on close).
    void saveSettings();

    void editorParameter(uint32_t id, double value) override;
    void editorGesture(uint32_t id, bool begin) override;
    void editorResize(int width, int height) override;

protected:
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    PluginEditorWindow(EditorState* state, Id clip, Id effect, QWidget* parent);
    bool start(QString* error);
    const Effect* effect() const;
    FrameTime localTime() const;
    void syncFromProject();

    EditorState* state_;
    Id clip_, effect_;
    std::unique_ptr<plugins::Instance> inst_;
    QWidget* host_ = nullptr;
    QTimer* idle_ = nullptr;
    std::map<uint32_t, int> gesture_;  // parameter -> serial of the drag in progress
    int gestureSerial_ = 0;
    bool resizing_ = false;
    int requestedW_ = 0, requestedH_ = 0;  // a size asked for while the editor was opening
    bool editorOpen_ = false;
};

}  // namespace montage
