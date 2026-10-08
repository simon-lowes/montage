// Montage — main window: panel layout, menus, shortcuts and wiring between
// the editor state, the two monitors and the timeline.
#pragma once

#include <QMainWindow>
#include <QTimer>
#include <vector>

#include "Recovery.h"
#include "TimelineWidget.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "media/Image.h"

class QAction;
class QDockWidget;
class QLabel;
class QMenu;

namespace montage {

class EditorState;
class PlaybackController;
class MonitorPanel;
class MediaBinWidget;
class RenderQueue;
class EffectsBrowser;
class InspectorWidget;
class ScopesWidget;
class MixerPanel;
class CaptionsPanel;
class TranscriptPanel;
class ShotSearchPanel;
class MulticamPanel;
class AudioMeterWidget;
class LoudnessReadout;
class VoiceoverDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    EditorState* state() const { return state_; }
    TimelineWidget* timeline() const { return timeline_; }
    RenderQueue* renderQueue() const { return queue_; }
    bool openProject(const QString& path);
    // Waits for pending renders and saves a screenshot of the window (testing aid).
    void scheduleScreenshot(const QString& path, int delayMs);
    // Brings a panel to the front by its object name ("inspector", "scopes", "mixer", "effects"...).
    void raisePanel(const QString& name);
    // Shot matching: remembers the Program monitor's picture at the playhead as
    // the look to match, then grades the selected clips to it (a Color Correct
    // first in each one's effects, replacing an earlier match). Returns how
    // many clips were matched.
    void setColourReference();
    bool hasColourReference() const { return !colourRef_.empty(); }
    int matchColour();
    // Split-screen compare in the Program monitor: the reference beside the current frame.
    void setCompareWithReference(bool on);
    void scriptCutDialog();  // Sequence › Build Cut from Script…
    // Music: markers on the music clip's bars or beats (returns how many), and
    // re-editing it to a length by whole bars (false if it could not).
    // Match Voice: the selected (or playing) audio clip's voice as the reference, then
    // EQ the selected audio clips to sound like it. Returns how many were matched.
    bool setVoiceReference();
    bool hasVoiceReference() const { return !voiceRef_.empty(); }
    int matchVoice();
    int addBeatMarkers(bool everyBeat);
    bool fitMusicToLength(FrameTime target);
    void fitMusicDialog();
    // Auto Reframe: a copy of the current sequence at aspect w:h with every
    // picture clip following its subject (speed 0 slower, 1 default, 2 faster),
    // made current; 0 if cancelled or nothing to do. The dialog asks for both.
    Id autoReframeSequence(int aspectW, int aspectH, int speed);
    void autoReframeDialog();
    // Reframes the selected clips within the current sequence; returns how many.
    int autoReframeClips(int speed = 1);
    // Editing staples (each one undo step; false/0 with a message when nothing applies).
    bool rippleTrimToPlayhead(bool previous);         // Q / W
    bool pasteAttributes(unsigned what);              // edit::Attribute flags, from the copied clips
    bool removeAttributes(unsigned what);
    bool addFrameHold();                              // the clip under the playhead
    bool replaceWithSource();                         // the selected clip, or the one under the playhead
    bool fitToFill();
    int selectForward(bool allTracks);
    // Render cache: renders the frames between In and Out (the whole sequence
    // without marks) as the Program monitor shows them; returns how many were
    // rendered (-1 if cancelled). The render bar follows edits by itself;
    // refreshRenderBar(true) brings it up to date before returning.
    int renderInToOut();
    void deleteRenderFiles();
    void refreshRenderBar(bool wait = false);

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
    void detectScenes();
    void normalizeLoudness();
    void autoColor();
    // Asks which attributes (Paste Attributes / Remove Attributes); 0 if cancelled.
    unsigned askAttributes(const QString& title, bool removing);
    // The video clip a clip command acts on: the selected one under the playhead, else the
    // top one under the playhead on the targeted track (or any).
    const Clip* clipForCommand() const;
    // The music clip a music command acts on: the selected audio clip, else the one under the playhead.
    const Clip* musicClip() const;
    void syncByAudio();
    enum class Interchange { Edl, Otio, Fcp7Xml, FcpXml };
    void exportInterchange(Interchange format);
    // The audio tracks as AAF with mono WAVs beside it (for Pro Tools, Fairlight), in the background with progress.
    void exportAafDialog();
    // Find Shots for moments that look like the selected clip (or the one under the playhead) at the playhead.
    void findSimilarShots();
public:
    bool exportAafTo(const QString& path, QString* summary = nullptr);
private:
    // Imports an OTIO, EDL or FCP XML timeline as a new sequence (one undo step).
    void importTimeline();
    void addTitle();
    void newSequence();
    void openInSource(Id media);
    Id makeSubclip();
    void applyFromBrowser(const QString& type, EffectCategory category);
    void scanPluginsInBackground();
    void offerRecovery(const std::vector<RecoveryManager::Session>& crashed);
    void openSnapshot();
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
    MulticamPanel* multicam_ = nullptr;
    CaptionsPanel* captions_ = nullptr;
    TranscriptPanel* transcript_ = nullptr;
    ShotSearchPanel* shots_ = nullptr;
    AudioMeterWidget* meter_ = nullptr;
    LoudnessReadout* loudness_ = nullptr;
    VoiceoverDialog* voiceover_ = nullptr;
    QTimer renderBarTimer_;
    int renderBarGeneration_ = 0;
    std::vector<QDockWidget*> docks_;
    QDockWidget* sourceDock_ = nullptr;
    QDockWidget* programDock_ = nullptr;
    QDockWidget* inspectorDock_ = nullptr;
    QDockWidget* binDock_ = nullptr;
    QDockWidget* effectsDock_ = nullptr;
    QDockWidget* scopesDock_ = nullptr;
    QDockWidget* mixerDock_ = nullptr;
    QDockWidget* captionsDock_ = nullptr;
    QDockWidget* transcriptDock_ = nullptr;
    QDockWidget* shotsDock_ = nullptr;
    QDockWidget* multicamDock_ = nullptr;
    QDockWidget* queueDock_ = nullptr;
    QDockWidget* keyframesDock_ = nullptr;
    QDockWidget* meterDock_ = nullptr;
    RenderQueue* queue_ = nullptr;
    Image colourRef_;       // the look Match Colour aims for
    QString colourRefName_;
    QImage colourRefView_;  // the reference at display size, for the compare
    std::vector<double> voiceRef_;  // Match Voice's reference spectrum
    QString voiceRefName_;
    QAction* compareRef_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    QLabel* statusInfo_ = nullptr;
    Monitor active_ = Monitor::Program;
    std::vector<edit::ClipboardItem> clipboard_;
    Id sourceSequence_ = 0;
    RecoveryManager* recovery_ = nullptr;
    QTimer syncTimer_;

    // Actions that change enabled state.
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* snapping_ = nullptr;
    std::vector<std::pair<TimelineWidget::Tool, QAction*>> toolActions_;
};

}  // namespace montage
