// Montage — main window: panel layout, menus, shortcuts and wiring between
// the editor state, the two monitors and the timeline.
#pragma once

#include <QMainWindow>
#include <QTimer>
#include <vector>

#include "TimelineWidget.h"
#include "core/EditOps.h"
#include "core/Effects.h"

class QAction;
class QDockWidget;
class QLabel;
class QMenu;

namespace montage {

class EditorState;
class PlaybackController;
class MonitorPanel;
class MediaBinWidget;
class EffectsBrowser;
class InspectorWidget;
class ScopesWidget;
class MixerPanel;
class AudioMeterWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    EditorState* state() const { return state_; }
    TimelineWidget* timeline() const { return timeline_; }
    bool openProject(const QString& path);
    // Waits for pending renders and saves a screenshot of the window (testing aid).
    void scheduleScreenshot(const QString& path, int delayMs);
    // Brings a panel to the front by its object name ("inspector", "scopes", "mixer", "effects"...).
    void raisePanel(const QString& name);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    enum class Monitor { Source, Program };

    void buildPanels();
    void buildActions();
    void buildMenus();
    void restoreLayout();
    void resetLayout();
    void syncProgram();
    void rebuildSourceProject();
    void updateTitle();
    void updateActions();
    PlaybackController* activeController() const;

    // Commands.
    bool maybeSave();
    void newProject();
    void openDialog();
    bool save();
    bool saveAs();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void exportMedia();
    void exportFrame();
    void copySelection(bool cut);
    void paste(bool insertMode);
    void deleteSelection(bool ripple);
    void addEdit(bool allTracks);
    void addDefaultTransition(bool audio);
    void speedDialog();
    void nudge(int frames);
    void matchFrame();
    void addMarker();
    void jumpMarker(bool forward);
    void markClip();
    void addTitle();
    void newSequence();
    void openInSource(Id media);
    void applyFromBrowser(const QString& type, EffectCategory category);
    void showShortcuts();
    void about();

    EditorState* state_;
    PlaybackController* program_;
    PlaybackController* source_;
    TimelineWidget* timeline_ = nullptr;
    MonitorPanel* sourcePanel_ = nullptr;
    MonitorPanel* programPanel_ = nullptr;
    MediaBinWidget* bin_ = nullptr;
    EffectsBrowser* effects_ = nullptr;
    InspectorWidget* inspector_ = nullptr;
    ScopesWidget* scopes_ = nullptr;
    MixerPanel* mixer_ = nullptr;
    AudioMeterWidget* meter_ = nullptr;
    std::vector<QDockWidget*> docks_;
    QDockWidget* sourceDock_ = nullptr;
    QDockWidget* programDock_ = nullptr;
    QDockWidget* inspectorDock_ = nullptr;
    QDockWidget* binDock_ = nullptr;
    QDockWidget* effectsDock_ = nullptr;
    QDockWidget* scopesDock_ = nullptr;
    QDockWidget* mixerDock_ = nullptr;
    QDockWidget* meterDock_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    QLabel* statusInfo_ = nullptr;
    Monitor active_ = Monitor::Program;
    std::vector<edit::ClipboardItem> clipboard_;
    Id sourceSequence_ = 0;
    QTimer autosave_;
    QTimer syncTimer_;

    // Actions that change enabled state.
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* snapping_ = nullptr;
    std::vector<std::pair<TimelineWidget::Tool, QAction*>> toolActions_;
};

}  // namespace montage
