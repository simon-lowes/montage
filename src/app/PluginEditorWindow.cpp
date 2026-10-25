#include "PluginEditorWindow.h"

#include <QCloseEvent>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <vector>

#include "EditorState.h"
#include "audio/AraHost.h"
#include "audio/PluginEffect.h"
#include "core/EditOps.h"

namespace montage {

namespace {
std::vector<PluginEditorWindow*>& openWindows() {
    static std::vector<PluginEditorWindow*> windows;
    return windows;
}
const std::string kParamPrefix = "param.";
}  // namespace

PluginEditorWindow* PluginEditorWindow::find(Id clip, Id effect) {
    for (PluginEditorWindow* w : openWindows())
        if (w->clip_ == clip && w->effect_ == effect) return w;
    return nullptr;
}

PluginEditorWindow* PluginEditorWindow::open(EditorState* state, Id clip, Id effect, QWidget* parent, QString* error) {
    if (PluginEditorWindow* w = find(clip, effect)) {
        w->show();
        w->raise();
        w->activateWindow();
        return w;
    }
    auto* w = new PluginEditorWindow(state, clip, effect, parent);
    if (!w->start(error)) {
        delete w;
        return nullptr;
    }
    w->show();
    w->raise();
    return w;
}

PluginEditorWindow::PluginEditorWindow(EditorState* state, Id clip, Id effect, QWidget* parent)
    : QWidget(parent, Qt::Window), state_(state), clip_(clip), effect_(effect) {
    setAttribute(Qt::WA_DeleteOnClose);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    // The plugin draws into this native child window.
    host_ = new QWidget(this);
    host_->setAttribute(Qt::WA_NativeWindow);
    host_->setAttribute(Qt::WA_DontCreateNativeAncestors);
    lay->addWidget(host_);
    openWindows().push_back(this);
}

PluginEditorWindow::~PluginEditorWindow() {
    auto& v = openWindows();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
    if (instance() && editorOpen_) instance()->closeEditor();
}

plugins::Instance* PluginEditorWindow::instance() const { return ara_ ? ara_->instance() : inst_.get(); }

const Effect* PluginEditorWindow::effect() const {
    const Sequence* s = state_->sequence();
    return s ? edit::ownedEffect(const_cast<Sequence&>(*s), clip_, effect_) : nullptr;
}

FrameTime PluginEditorWindow::localTime() const {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    if (const Clip* c = edit::clipById(*s, clip_))
        return std::clamp<FrameTime>(state_->playhead() - c->start, 0, std::max<FrameTime>(0, c->duration - 1));
    return state_->playhead();  // track, bus and master effects keyframe on the timeline
}

bool PluginEditorWindow::start(QString* error) {
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    const Effect* e = effect();
    if (!e || (e->type != "plugin" && e->type != "ara")) return fail(tr("This effect is not a plugin"));
    const auto d = plugins::Registry::instance().find(e->s("plugin_id"));
    if (!d) return fail(tr("The plugin is not installed or has not been scanned"));
    std::string err;
    if (e->type == "ara") {
        // An ARA plug-in edits the clip's document: its editor view and renderer bound to the clip's sound.
        const Sequence* s = state_->sequence();
        const Clip* c = s ? edit::clipById(*s, clip_) : nullptr;
        std::string key;
        AudioBufferPtr sound;
        if (!c || !ara::clipSource(state_->project(), *s, *c, key, sound, &err) || !(ara_ = ara::liveSession(*e, key, sound, &err)))
            return fail(tr("The ARA plug-in could not open the clip: %1").arg(QString::fromStdString(err)));
        araArchive_ = e->s("archive");
    } else {
        inst_ = plugins::instantiate(*d, &err);
        if (!inst_) return fail(tr("The plugin could not be loaded: %1").arg(QString::fromStdString(err)));
        const std::string saved = plugins::decodeState(e->s("state"));
        if (!saved.empty()) inst_->loadState(saved);
    }
    plugins::Instance* inst = instance();
    for (const plugins::ParamInfo& p : inst->parameters()) params_[p.id] = p;
    syncFromProject();
    if (!inst->hasEditor()) return fail(tr("%1 has no editor of its own; use its controls in the Inspector")
                                            .arg(QString::fromStdString(d->name)));
    int w = 400, h = 300;
    void* parent = reinterpret_cast<void*>(host_->winId());
    if (!inst->openEditor(parent, this, w, h)) return fail(tr("The plugin's editor could not be opened"));
    editorOpen_ = true;
    if (requestedW_ > 0 && requestedH_ > 0) {
        w = requestedW_;
        h = requestedH_;
    }
    setWindowTitle(QStringLiteral("%1 — %2").arg(QString::fromStdString(d->name), QString::fromStdString(plugins::effectName(*e))));
    if (!inst->editorResizable()) {
        host_->setFixedSize(std::max(1, w), std::max(1, h));
        adjustSize();
    } else {
        resize(std::max(1, w), std::max(1, h));  // the layout has no margins: the editor fills the window
    }
    idle_ = new QTimer(this);
    connect(idle_, &QTimer::timeout, this, [this] {
        if (ara_) ara_->idle();
        else inst_->idle();
    });
    idle_->start(30);
    // Settings are saved into the project every few seconds and on close.
    auto* saver = new QTimer(this);
    connect(saver, &QTimer::timeout, this, [this] {
        if (!state_->inGesture()) saveSettings();  // not in the middle of a drag (an edit would end it)
    });
    saver->start(3000);
    connect(state_, &EditorState::projectChanged, this, [this] {
        if (!effect()) {
            close();  // the effect or its clip is gone (deleted, or undone)
            return;
        }
        if (ara_ && effect()->s("archive") != araArchive_) {
            discard_ = true;  // an undo or a reset replaced the document: this one is out of date
            close();
            return;
        }
        syncFromProject();
    });
    connect(state_, &EditorState::playheadChanged, this, [this] { syncFromProject(); });
    return true;
}

void PluginEditorWindow::syncFromProject() {
    const Effect* e = effect();
    plugins::Instance* inst = instance();
    if (!e || !inst) return;
    // Show the parameter values the project has at the playhead (automation, Inspector edits, undo).
    const FrameTime lt = localTime();
    for (const auto& [key, param] : e->params) {
        if (key.rfind(kParamPrefix, 0) != 0) continue;
        const uint32_t id = uint32_t(std::strtoul(key.c_str() + kParamPrefix.size(), nullptr, 10));
        if (gesture_.count(id)) continue;  // the user is dragging it right now
        const auto live = state_->liveParameter(clip_, effect_, key);  // a mixer pass writing it
        const double v = live ? *live : param.at(lt);
        if (std::fabs(inst->parameter(id) - v) > 1e-9) inst->setParameter(id, v);
    }
}

void PluginEditorWindow::editorParameter(uint32_t id, double value) {
    const FrameTime lt = localTime();
    const Id clip = clip_, eff = effect_;
    const std::string key = kParamPrefix + std::to_string(id);
    // On a track or bus whose automation the mixer is writing, the move goes to that pass.
    if (const Effect* e = effect()) {
        const auto info = params_.find(id);
        const bool known = info != params_.end();
        const double range = known ? info->second.max - info->second.min : 1;
        const auto stored = e->params.find(key);
        const double before = stored != e->params.end() ? stored->second.at(lt) : known ? info->second.def : value;
        // Held while the plugin reports the knob grabbed; a plugin that reports no grab counts as held briefly. A
        // stepped parameter's keys hold, at its own (often 0 to 1) values.
        std::function<bool()> held;
        if (gesture_.count(id)) held = [self = QPointer<PluginEditorWindow>(this), id] { return self && self->gesture_.count(id) > 0; };
        if (state_->captureParameter({clip, eff, key, value, before, std::max(1e-9, std::fabs(range) * 0.001), known && info->second.stepped, held}))
            return;
    }
    const auto g = gesture_.find(id);
    const QString merge = QStringLiteral("plugin-%1-%2-%3").arg(eff).arg(id).arg(g != gesture_.end() ? g->second : 0);
    state_->edit(tr("Change Plugin Parameter"), [clip, eff, key, value, lt](Project&, Sequence& s) {
        Effect* e = edit::ownedEffect(s, clip, eff);
        if (!e || (e->params.count(key) && std::fabs(e->params[key].at(lt) - value) < 1e-12)) return false;
        e->params[key].set(lt, value);
        return true;
    }, merge);
}

void PluginEditorWindow::editorGesture(uint32_t id, bool begin) {
    if (begin) gesture_[id] = ++gestureSerial_;
    else gesture_.erase(id);
}

void PluginEditorWindow::editorResize(int width, int height) {
    if (!editorOpen_) {
        requestedW_ = width;
        requestedH_ = height;
        return;
    }
    resizing_ = true;
    if (host_->minimumWidth() == host_->maximumWidth()) {
        host_->setFixedSize(std::max(1, width), std::max(1, height));
        adjustSize();
    } else {
        resize(std::max(1, width), std::max(1, height));
    }
    resizing_ = false;
}

void PluginEditorWindow::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (!resizing_ && editorOpen_ && instance() && instance()->editorResizable()) instance()->setEditorSize(host_->width(), host_->height());
}

void PluginEditorWindow::saveSettings() {
    const Effect* e = effect();
    plugins::Instance* inst = instance();
    if (!e || !inst || discard_) return;
    if (ara_) ara_->idle();
    else inst->idle();
    const std::string state = plugins::encodeState(inst->saveState());
    // An ARA plug-in's edits live in its document, kept with the project as an archive when it reports a change.
    const std::string archive = ara_ && ara_->changed() ? plugins::encodeState(ara_->archive()) : std::string();
    const bool newState = !state.empty() && state != e->s("state");
    const bool newArchive = ara_ && !archive.empty() && archive != e->s("archive");
    if (!newState && !newArchive) return;
    const Id clip = clip_, eff = effect_;
    if (newArchive) {
        araArchive_ = archive;  // (before the edit: its projectChanged must find the document current)
        ara::noteArchiveSaved(eff, archive);
    }
    state_->edit(tr("Plugin Settings"), [clip, eff, state, archive, newState, newArchive](Project&, Sequence& s) {
        Effect* fx = edit::ownedEffect(s, clip, eff);
        if (!fx) return false;
        if (newState) fx->strings["state"] = state;
        if (newArchive) fx->strings["archive"] = archive;
        return true;
    }, QStringLiteral("plugin-state-%1").arg(eff));
}

void PluginEditorWindow::closeEvent(QCloseEvent* e) {
    if (editorOpen_) {
        saveSettings();
        instance()->closeEditor();
        editorOpen_ = false;
    }
    QWidget::closeEvent(e);
    if (parentWidget()) parentWidget()->window()->activateWindow();  // back to the editor
}

}  // namespace montage
